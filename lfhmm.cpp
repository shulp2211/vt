/* The MIT License

   Copyright (c) 2014 Adrian Tan <atks@umich.edu>

   Permission is hereby granted, free of charge, to any person obtaining a copy
   of this software and associated documentation files (the "Software"), to deal
   in the Software without restriction, including without limitation the rights
   to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
   copies of the Software, and to permit persons to whom the Software is
   furnished to do so, subject to the following conditions:

   The above copyright notice and this permission notice shall be included in
   all copies or substantial portions of the Software.

   THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
   IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
   FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
   AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
   LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
   OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
   THE SOFTWARE.
*/

#include "lfhmm.h"

#define MAXLEN 256
#define MAXLEN_NBITS 8

#define S   0
#define M   1
#define D   2
#define I   3
#define Z   4
#define E   5
#define N   6
#define TBD 7
#define NSTATES 6

#define MOTIF     0
#define READ      1
#define UNMODELED 2
#define UNCERTAIN 3

//match type
#define MATCH      0
#define READ_ONLY  1
#define PROBE_ONLY 2

/*for indexing single array*/
#define index(i,j) (((i)<<MAXLEN_NBITS)+(j))

/*functions for getting trace back information*/
#define track_get_u(t)    (((t)&0xFF000000)>>24)
#define track_get_d(t)    (((t)&0x00FF0000)>>16)
#define track_get_c(t)    (((t)&0x0000FF00)>>8)
#define track_get_p(t)    (((t)&0x000000FF))
#define track_get_base(t) (model[track_get_d(t)][track_get_p(t)-1])
#define track_valid(t) ((track_get_d(t)==MOTIF)&&track_get_p(t)!=0)
#define track_set_u(t,u)  (((t)&0x00FFFFFF)|((u)<<24))
#define track_set_d(t,d)  (((t)&0xFF00FFFF)|((d)<<16))
#define track_set_c(t,c)  (((t)&0xFFFF00FF)|((c)<<8))
#define track_set_p(t,p)  (((t)&0xFFFFFF00)|(p))
#define make_track(u,d,c,p) (((u)<<24)|((d)<<16)|((c)<<8)|(p))

//[]
#define NULL_TRACK  0x0F040000
//[N|l|0|0]
#define START_TRACK 0x0F000000

/**
 * Constructor.
 */
LFHMM::LFHMM()
{
    lt = new LogTool();
};

/**
 * Constructor.
 */
LFHMM::LFHMM(LogTool *lt)
{
    this->lt = lt;
};

/**
 * Destructor.
 */
LFHMM::~LFHMM()
{
    delete optimal_path;

    //the best alignment V_ for subsequence (i,j)
    for (size_t state=S; state<=Z; ++state)
    {
        delete V[state];
        delete U[state];
    }

    delete V;
    delete U;
};

/**
 * Initializes object, helper function for constructor.
 */
void LFHMM::initialize(const char* motif)
{
    model = new char*[3];
    model[MOTIF] = strdup(motif);

    mlen = strlen(model[MOTIF]);

    optimal_path = new int32_t[MAXLEN<<2];
    optimal_path_traced = false;

    delta = 0.001;
    epsilon = 0.05;
    tau = 0.01;
    eta = 0.01;

    log10OneSixteenth = log10(1.0/16.0);
    log10OneSixteenth = log10(0.03160696);

    for (size_t i=S; i<=Z; ++i)
    {
        for (size_t j=S; j<=Z; ++j)
        {
            T[i][j] = -INFINITY;
        }
    }

    T[S][M] = log10((1-2*delta-tau)/(eta*(1-eta)*(1-eta)));
    T[M][M] = log10(((1-2*delta-tau))/((1-eta)*(1-eta)));
    T[D][M] = log10(((1-epsilon-tau))/((1-eta)*(1-eta)));
    T[I][M] = T[I][M];

    T[S][D] = log10(delta/(eta*(1-eta)));
    T[M][D] = log10(delta/(1-eta));
    T[D][D] = log10(delta/(1-eta));

    T[S][I] = log10(delta/(eta*(1-eta)));
    T[M][I] = log10(delta/(1-eta));
    T[I][I] = log10(delta/(1-eta));

    T[M][Z] = log10(tau/(eta*(1-eta)));
    T[D][Z] = log10(tau/(eta*(1-eta)));
    T[I][Z] = log10(tau/(eta*(1-eta)));
    T[Z][Z] = 0;

    typedef int32_t (LFHMM::*move) (int32_t t, int32_t j);
    V = new double*[NSTATES];
    U = new int32_t*[NSTATES];
    moves = new move*[NSTATES];
    for (size_t state=S; state<=Z; ++state)
    {
        V[state] = new double[MAXLEN*MAXLEN];
        U[state] = new int32_t[MAXLEN*MAXLEN];
        moves[state] = new move[NSTATES];
    }

    for (size_t state=S; state<E; ++state)
    {
        moves[state] = new move[NSTATES];
    }

    moves[S][M] = &LFHMM::move_S_M;
    moves[M][M] = &LFHMM::move_M_M;
    moves[D][M] = &LFHMM::move_D_M;
    moves[I][M] = &LFHMM::move_I_M;
    moves[S][D] = &LFHMM::move_S_D;
    moves[M][D] = &LFHMM::move_M_D;
    moves[D][D] = &LFHMM::move_D_D;
    moves[S][I] = &LFHMM::move_S_I;
    moves[M][I] = &LFHMM::move_M_I;
    moves[I][I] = &LFHMM::move_I_I;

    moves[M][Z] = &LFHMM::move_M_Z;
    moves[D][Z] = &LFHMM::move_D_Z;
    moves[I][Z] = &LFHMM::move_I_Z;
    moves[Z][Z] = &LFHMM::move_Z_Z;

    //used for back tracking, this points to the state prior to the alignment for subsequence (i,j)
    //that ends with the corresponding state

    int32_t t=0;
    for (size_t i=0; i<MAXLEN; ++i)
    {
        for (size_t j=0; j<MAXLEN; ++j)
        {
            size_t c = index(i,j);

            V[S][c] = -INFINITY;
            U[S][c] = NULL_TRACK;

            //M
            V[M][c] = -INFINITY;
            if (!i || !j)
            {
                U[M][c] = make_track(N,UNMODELED,0,0);
            }
            else
            {
                U[M][c] = make_track(TBD,UNCERTAIN,0,0);
            }

            //D
            V[D][c] = -INFINITY;
            if (!i || !j)
            {
                U[D][c] = make_track(N,UNMODELED,0,0);
            }
            else
            {
                U[D][c] = make_track(TBD,UNCERTAIN,0,0);
            }

            //I
            V[I][c] = -INFINITY;
            if (!i || !j)
            {
                U[I][c] = make_track(N,UNMODELED,0,0);
            }
            else
            {
                U[I][c] = make_track(TBD,UNCERTAIN,0,0);
            }

            //Z
            V[Z][c] = -INFINITY;
            if (!i || !j)
            {
                U[Z][c] = make_track(N,UNMODELED,0,0);
            }
            else
            {
                U[Z][c] = make_track(TBD,UNCERTAIN,0,0);
            }

        }
    }

    logEta = log10(eta);
    logTau = log10(tau);

    V[S][index(0,0)] = 0;
    U[S][index(0,0)] = START_TRACK;

    V[M][index(0,0)] = -INFINITY;
    V[Z][index(0,0)] = -INFINITY;
};

/**
 * Computes the score associated with the move from A to B
 * Updates the max_score and associated max_track.
 *
 * @A      - start state
 * @B      - end state
 * @index1 - flattened index of the one dimensional array of start state
 * @j      - 1 based position of read of start state
 * @m      - base match required (MATCH, PROBE_ONLY, READ_ONLY)
 */
void LFHMM::proc_comp(int32_t A, int32_t B, int32_t index1, int32_t j, int32_t match_type)
{
    double emission = 0, score = 0, valid = 0;

    //t is the new track
    int32_t  t =  (this->*(moves[A][B]))(U[A][index1],j);

    if (t==NULL_TRACK)
    {
        valid = -INFINITY;
    }
    else if (match_type==MATCH)
    {
          emission = log10_emission_odds(track_get_base(t), read[j], qual[j]-33);
    }

    score = V[A][index1] + T[A][B] + emission + valid;

    if (score>max_score)
    {
        max_score = score;
        max_track = t;
    }

    if (1)
    {
        std::cerr << "\t" << state2string(A) << "=>" << state2string(B);
        std::cerr << " (" << ((index1-j)>>MAXLEN_NBITS) << "," << j << ") ";
        std::cerr << track2string(U[A][index1]) << "=>";
        std::cerr << track2string(t) << " ";
        std::cerr << emission << " (e: " << (track_get_d(t)<=MOTIF?track_get_base(t):'N') << " vs " << (j!=rlen?read[j]:'N')  << ") + ";
        std::cerr << T[A][B] << " (t) + ";
        std::cerr << V[A][index1] << " (p) + ";
        std::cerr << valid << " (v) = ";
        std::cerr << score << "\n";
    }
}

/**
 * Align y against x.
 */
void LFHMM::align(const char* read, const char* qual, bool debug)
{
    optimal_path_traced = false;
    this->read = read;
    this->qual = qual;
    rlen = strlen(read);
    plen = rlen;
    
    if (rlen>MAXLEN)
    {
        fprintf(stderr, "[%s:%d %s] Sequence to be aligned is greater than %d currently supported: %d\n", __FILE__, __LINE__, __FUNCTION__, MAXLEN, rlen);
        exit(1);
    }

    double max = 0;
    char maxPath = 'X';

    size_t c,d,u,l;

    debug = true;

    //alignment
    //take into consideration
    for (size_t i=1; i<=plen; ++i)
    {
        //break;

        for (size_t j=1; j<=rlen; ++j)
        {
            c = index(i,j);
            d = index(i-1,j-1);
            u = index(i-1,j);
            l = index(i,j-1);

            if (debug) std::cerr << "(" << i << "," << j << ")";

            /////
            //M//
            /////
            //only need to update this i>rflen
            max_score = -INFINITY;
            max_track = NULL_TRACK;
            proc_comp(S, M, d, j-1, MATCH);
            proc_comp(M, M, d, j-1, MATCH);
            proc_comp(D, M, d, j-1, MATCH);
            proc_comp(I, M, d, j-1, MATCH);
            V[M][c] = max_score;
            U[M][c] = max_track;
            if (debug) std::cerr << "\tset M " << max_score << " - " << track2string(max_track) << "\n";

            /////
            //D//
            /////
            max_score = -INFINITY;
            max_track = NULL_TRACK;
            proc_comp(S, D, u, j, PROBE_ONLY);
            proc_comp(M, D, u, j, PROBE_ONLY);
            proc_comp(D, D, u, j, PROBE_ONLY);
            V[D][c] = max_score;
            U[D][c] = max_track;
            if (debug) std::cerr << "\tset D " << max_score << " - " << track2string(max_track) << "\n";

            /////
            //I//
            /////
            max_score = -INFINITY;
            max_track = NULL_TRACK;
            proc_comp(S, I, l, j-1, READ_ONLY);
            proc_comp(M, I, l, j-1, READ_ONLY);
            proc_comp(I, I, l, j-1, READ_ONLY);
            V[I][c] = max_score;
            U[I][c] = max_track;
            if (debug) std::cerr << "\tset I " << max_score << " - " << track2string(max_track) << "\n";

            //////
            //Z//
            //////
            max_score = -INFINITY;
            max_track = NULL_TRACK;            
            proc_comp(M, Z, l, j-1, READ_ONLY);
            proc_comp(D, Z, l, j-1, READ_ONLY);
            proc_comp(I, Z, l, j-1, READ_ONLY);
            proc_comp(Z, Z, l, j-1, READ_ONLY);
            V[Z][c] = max_score;
            U[Z][c] = max_track;
            if (debug) std::cerr << "\tset Z " << max_score << " - " << track2string(max_track) << "\n";

        }
    }

    if (1)
    {
        std::cerr << "\n   =V[S]=\n";
        print(V[S], plen+1, rlen+1);
        std::cerr << "\n   =U[S]=\n";
        print_U(U[S], plen+1, rlen+1);

        std::cerr << "\n   =V[M]=\n";
        print(V[M], plen+1, rlen+1);
        std::cerr << "\n   =U[M]=\n";
        print_U(U[M], plen+1, rlen+1);
        std::cerr << "\n   =V[D]=\n";
        print(V[D], plen+1, rlen+1);
        std::cerr << "\n   =U[D]=\n";
        print_U(U[D], plen+1, rlen+1);
        std::cerr << "\n   =V[I]=\n";
        print(V[I], plen+1, rlen+1);
        std::cerr << "\n   =U[I]=\n";
        print_U(U[I], plen+1, rlen+1);

        std::cerr << "\n   =V[Z]=\n";
        print(V[Z], plen+1, rlen+1);
        std::cerr << "\n   =U[Z]=\n";
        print_U(U[Z], plen+1, rlen+1);

        std::cerr << "\n";
    }

    trace_path();
};

/**
 * Trace path after alignment.
 */
void LFHMM::trace_path()
{
    //search for a complete path in M or W or Z
    size_t c;
    optimal_score = -INFINITY;
    optimal_track = NULL_TRACK;
    optimal_state = TBD;
    optimal_probe_len = 0;
    for (size_t i=0; i<=plen; ++i)
    {
        c = index(i,rlen);
        if (V[Z][c]>=optimal_score)
        {
            optimal_score = V[Z][c];
            optimal_track = U[Z][c];
            optimal_state = Z;
            optimal_probe_len = i;
        }
    }

    //trace path
    optimal_path_ptr = optimal_path+(MAXLEN<<2)-1;
    int32_t i=optimal_probe_len, j=rlen;
    int32_t last_t = make_track(optimal_state, MOTIF, 0, mlen+1);
    optimal_path_len = 0;
    int32_t u;
    do
    {
        u = track_get_u(last_t);
        last_t = U[u][index(i,j)];
        *optimal_path_ptr = track_set_u(last_t, u);

        std::cerr << track2string(*optimal_path_ptr) << " (" << i << "," << j << ")\n";

        if (u==M)
        {
            --i; --j;
        }
        else if (u==D)
        {
            --i;
        }
        else if (u==I || u==Z)
        {
            --j;
        }

        --optimal_path_ptr;
        ++optimal_path_len;
    } while (track_get_u(last_t)!=S);

    ++optimal_path_ptr;
    optimal_path_traced = true;
};

/**
 * Compute log10 emission odds based on equal error probability distribution.
 */
double LFHMM::log10_emission_odds(char probe_base, char read_base, uint32_t pl)
{
    //4 encodes for N
    if (read_base=='N' || probe_base=='N')
    {
        //silent match
        return -INFINITY;
    }

    if (read_base!=probe_base)
    {
        return lt->pl2log10_varp(pl);
    }
    else
    {
        return -lt->pl2log10_varp(pl);
    }
};

/**
 * Converts state to string representation.
 */
std::string LFHMM::state2string(int32_t state)
{
    if (state==S)
    {
        return "S";
    }

    else if (state==M)
    {
        return "M";
    }
    else if (state==D)
    {
        return "D";
    }
    else if (state==I)
    {
        return "I";
    }
    else if (state==Z)
    {
        return "Z";
    }
    else if (state==E)
    {
        return "E";
    }
    else if (state==N)
    {
        return "N";
    }
    else if (state==TBD)
    {
        return "*";
    }
    else
    {
        return "!";
    }
}

/**
 * Converts state to cigar string representation.
 */
std::string LFHMM::state2cigarstring(int32_t state)
{
    if (state==S)
    {
        return "S";
    }
    else if (state==M)
    {
        return "M";
    }
    else if (state==D)
    {
        return "D";
    }
    else if (state==I)
    {
        return "I";
    }
    else if (state==Z)
    {
        return "Z";
    }
    else if (state==E)
    {
        return "E";
    }
    else if (state==N)
    {
        return "N";
    }
    else if (state==TBD)
    {
        return "*";
    }
    else
    {
        return "!";
    }
}

/**
 * Converts state to cigar string representation.
 */
std::string LFHMM::track2cigarstring1(int32_t t, int32_t j)
{
    int32_t state = track_get_u(t);

    if (state==S)
    {
        return "S";
    }
    else if (state==M)
    {
        if (track_get_base(t)==read[j-1])
        {
            return "M";
        }
        else
        {
            return "*";
        }
    }
    else if (state==D)
    {
        return "D";
    }
    else if (state==I)
    {
        return "I";
    }
    else if (state==Z)
    {
        return "Z";
    }
    else if (state==E)
    {
        return "E";
    }
    else if (state==N)
    {
        return "N";
    }
    else if (state==TBD)
    {
        return "*";
    }
    else
    {
        return "!";
    }
}

/**
 * Converts state to cigar string representation.
 */
std::string LFHMM::track2cigarstring2(int32_t t)
{
    int32_t state = track_get_u(t);

    if (state==M)
    {
        return (track_get_c(t)%2==0?"+":"o");
    }
    else if (state==D)
    {
        return (track_get_c(t)%2==0?"+":"o");
    }
    else if (state==I)
    {
        return (track_get_c(t)%2==0?"+":"o");
    }
    else
    {
        return " ";
    }
}
/**
 * Converts model component to string representation.
 */
std::string LFHMM::component2string(int32_t component)
{
    if (component==MOTIF)
    {
        return "m";
    }
    else if (component==UNMODELED)
    {
        return "!";
    }
    else if (component==READ)
    {
        return "s";
    }
    else if (component==UNCERTAIN)
    {
        return "?";
    }
    else
    {
        return "!";
    }
}

/**
 * Prints an alignment.
 */
void LFHMM::print_alignment()
{
    std::string pad = "\t";
    print_alignment(pad);
};

/**
 * Prints an alignment with padding.
 */
void LFHMM::print_alignment(std::string& pad)
{
    if (!optimal_path_traced)
    {
        std::cerr << "path not traced\n";
    }

    std::cerr << "repeat motif : " << model[MOTIF] << "\n";
    std::cerr << "plen         : " << plen << "\n";
    std::cerr << "\n";
    std::cerr << "read         : " << read << "\n";
    std::cerr << "rlen         : " << rlen << "\n";
    std::cerr << "\n";
    std::cerr << "optimal score: " << optimal_score << "\n";
    std::cerr << "optimal state: " << state2string(optimal_state) << "\n";
    std::cerr << "optimal track: " << track2string(optimal_track) << "\n";
    std::cerr << "optimal probe len: " << optimal_probe_len << "\n";
    std::cerr << "optimal path length : " << optimal_path_len << "\n";
    std::cerr << "optimal path     : " << optimal_path << "\n";
    std::cerr << "optimal path ptr : " << optimal_path_ptr  << "\n";
    std::cerr << "max j: " << rlen << "\n";

    //print path
    int32_t* path;
    path = optimal_path_ptr;
    std::cerr << "Model:  ";
    int32_t t = NULL_TRACK;
    int32_t j = 0;
    while (path<optimal_path+(MAXLEN<<2))
    {
        int32_t u = track_get_u(*path);
        if (u==M || u==D)
        {
            std::cerr << track_get_base(*path);
        }
        else
        {
            std::cerr << '-';
        }
        ++path;
    }
    std::cerr << " \n";

    std::cerr << "       S";
    path = optimal_path_ptr;
    j=1;
    while (path<optimal_path+(MAXLEN<<2))
    {
        std::cerr << track2cigarstring1(*path,j);
        int32_t u = track_get_u(*path);
        if (u==M || u==I || u==Z)
        {
            ++j;
        }
        ++path;
    }
    std::cerr << "E\n";

    path = optimal_path_ptr;
    std::cerr << "        ";
    while (path<optimal_path+(MAXLEN<<2))
    {
        std::cerr << track2cigarstring2(*path);
        ++path;
    }
    std::cerr << " \n";

    path = optimal_path_ptr;
    j=1;
    std::cerr << "Read:   ";
    while (path<optimal_path+(MAXLEN<<2))
    {
        int32_t u = track_get_u(*path);
        if (u==M || u==I || u==Z)
        {
            std::cerr << read[j-1];
            ++j;
        }
        else
        {
            std::cerr << '-';
        }
        ++path;
    }
    std::cerr << " \n";
};

/**
 * Prints a double matrix.
 */
void LFHMM::print(double *v, size_t plen, size_t rlen)
{
    double val;
    std::cerr << std::setprecision(1) << std::fixed;
    for (size_t i=0; i<plen; ++i)
    {
        for (size_t j=0; j<rlen; ++j)
        {
            val =  v[index(i,j)];
            std::cerr << (val<0?"  ":"   ") << val;
        }

        std::cerr << "\n";
    }
};

/**
 * Prints a char matrix.
 */
void LFHMM::print(int32_t *v, size_t plen, size_t rlen)
{
    double val;
    std::cerr << std::setprecision(1) << std::fixed << std::setw(6);
    for (size_t i=0; i<plen; ++i)
    {
        for (size_t j=0; j<rlen; ++j)
        {
          val =  v[index(i,j)];
          std::cerr << (val<0?"  ":"   ") << val;
        }

        std::cerr << "\n";
    }
};

/**
 * Prints the transition matrix.
 */
void LFHMM::print_T()
{
    for (size_t j=S; j<=Z; ++j)
    {
        std::cerr << std::setw(8) << std::setprecision(2) << std::fixed << state2string(j);
    }
    std::cerr << "\n";

    for (size_t i=S; i<=Z; ++i)
    {
        for (size_t j=S; j<=Z; ++j)
        {
            if (j)
            {
                std::cerr << std::setw(8) << std::setprecision(2) << std::fixed << T[i][j];
            }
            else
            {
                std::cerr << state2string(i) << std::setw(8) << std::setprecision(2) << std::fixed << T[i][j];
            }
        }
        std::cerr << "\n";
    }
};

/**
 * Prints U.
 */
void LFHMM::print_U(int32_t *U, size_t plen, size_t rlen)
{
    std::cerr << std::setprecision(1) << std::fixed;
    std::string state;
    for (size_t i=0; i<plen; ++i)
    {
        for (size_t j=0; j<rlen; ++j)
        {
            int32_t t = U[index(i,j)];
            state = state2string(track_get_u(t));
            std::cerr << (state.size()==1 ? "   " : "  ")
                      << state << "|"
                      << component2string(track_get_d(t)) << "|"
                      << track_get_c(t) << "|"
                      << track_get_p(t) << (j==rlen-1?"\n":"   ");
        }
    }
};

/**
 * Prints U and V.
 */
void LFHMM::print_trace(int32_t state, size_t plen, size_t rlen)
{
    std::cerr << std::setprecision(1) << std::fixed;
    int32_t *u = U[state];
    double *v = V[state];
    std::string s;
    for (size_t i=0; i<plen; ++i)
    {
        for (size_t j=0; j<rlen; ++j)
        {
            int32_t t = u[index(i,j)];
            s = state2string(track_get_u(t));
            std::cerr << (s.size()==1 ? "   " : "  ")
                      << s << "|"
                      << component2string(track_get_d(t)) << "|"
                      << track_get_c(t) << "|"
                      << track_get_p(t) << "|"
                      << v[index(i,j)];
        }

        std::cerr << "\n";
    }
};

/**
 * Returns a string representation of track.
 */
std::string LFHMM::track2string(int32_t t)
{
    ss.str("");
    ss << state2string(track_get_u(t)) <<"|"
       <<component2string(track_get_d(t)) <<"|"
       <<track_get_c(t) <<"|"
       <<track_get_p(t);

    return ss.str();
}

/**
 * Prints track.
 */
void LFHMM::print_track(int32_t t)
{
    std::cerr << track2string(t) << "\n";
}

#undef S
#undef M
#undef I
#undef D
#undef Z
#undef E
#undef N
#undef TBD
#undef NSTATES

#undef MOTIF
#undef READ
#undef UNMODELED
#undef UNCERTAIN

#undef MATCH
#undef READ_ONLY
#undef PROBE_ONLY

#undef index
#undef track_get_u
#undef track_get_d
#undef track_get_d
#undef track_get_c
#undef track_get_p
#undef track_get_base
#undef track_valid
#undef track_set_u
#undef track_set_d
#undef track_set_c
#undef track_set_p
#undef make_track

#undef NULL_TRACK
#undef START_TRACK